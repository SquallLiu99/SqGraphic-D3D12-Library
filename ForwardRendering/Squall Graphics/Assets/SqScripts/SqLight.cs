﻿using System;
using System.Runtime.InteropServices;
using UnityEngine;

/// <summary>
/// sq light
/// </summary>
[RequireComponent(typeof(Light))]
public class SqLight : MonoBehaviour
{
    public enum ShadowSize
    {
        S256 = 0, S512, S1024, S2048, S4096, S8192
    }

    [DllImport("SquallGraphics")]
    static extern int AddNativeLight(int _instanceID, SqLightData _sqLightData);

    [DllImport("SquallGraphics")]
    static extern void UpdateNativeLight(int _nativeID, SqLightData _sqLightData);

    [DllImport("SquallGraphics")]
    static extern void UpdateNativeShadow(int _nativeID, SqLightData _sqLightData);

    [DllImport("SquallGraphics")]
    static extern void InitNativeShadows(int _nativeID, int _numCascade, IntPtr[] _shadowMaps);

    [DllImport("SquallGraphics")]
    static extern void SetShadowViewPortScissorRect(int _nativeID, ViewPort _viewPort, RawRect _rawRect);

    [DllImport("SquallGraphics")]
    static extern void SetShadowFrustum(int _nativeID, Matrix4x4 _view, Matrix4x4 _projCulling, int _cascade);

    [StructLayout(LayoutKind.Sequential)]
    struct SqLightData
    {
        /// <summary>
        /// shadow matrix
        /// </summary>
        [MarshalAs(UnmanagedType.ByValArray, ArraySubType = UnmanagedType.Struct, SizeConst = 4)]
        public Matrix4x4[] shadowMatrix;

        /// <summary>
        /// color
        /// </summary>
        public Vector4 color;

        /// <summary>
        /// world dir as directional light, world pos for point/spotlight
        /// </summary>
        public Vector4 worldPos;

        /// <summary>
        /// type
        /// </summary>
        public int type;

        /// <summary>
        /// intensity
        /// </summary>
        public float intensity;

        /// <summary>
        /// cascade
        /// </summary>
        public int numCascade;

        /// <summary>
        /// padding
        /// </summary>
        public float padding;
    }

    /// <summary>
    /// opaque shadows
    /// </summary>
    public static RenderTexture collectShadows;

    /// <summary>
    /// shadow size
    /// </summary>
    [Header("Shadow size, note even cascade use this size!")]
    public ShadowSize shadowSize = ShadowSize.S4096;

    /// <summary>
    /// cascade setting
    /// </summary>
    [Header("Cascade setting")]
    public float[] cascadeSetting;

    /// <summary>
    /// shadow map
    /// </summary>
    public RenderTexture[] shadowMaps;

    /// <summary>
    /// collect result
    /// </summary>
    public RenderTexture collectResult;

    SqLightData lightData;
    Light lightCache;
    Camera shadowCam;
    Camera mainCam;
    Transform mainCamTrans;

    int nativeID = -1;
    int[] shadowMapSize = { 256, 512, 1024, 2048, 4096, 8192 };
    float[] cascadeLast;

    void Start()
    {
        // return if sqgraphic not init
        if (SqGraphicManager.instance == null)
        {
            enabled = false;
            return;
        }

        mainCam = Camera.main;
        mainCamTrans = mainCam.transform;
        InitNativeLight();
        InitShadows();
    }

    void Update()
    {
        UpdateShadowMatrix();
        UpdateNativeLight();

        // keep cascade
        for (int i = 0; i < cascadeSetting.Length; i++)
        {
            cascadeLast[i] = cascadeSetting[i];
        }

        collectResult = collectShadows;
    }

    void OnDestroy()
    {
        if (shadowCam)
        {
            shadowCam.targetTexture = null;
        }

        for (int i = 0; i < shadowMaps.Length; i++)
        {
            if (shadowMaps[i])
            {
                shadowMaps[i].Release();
                DestroyImmediate(shadowMaps[i]);
            }
        }

        if (collectShadows != null)
        {
            collectShadows.Release();
            DestroyImmediate(collectShadows);
        }
    }

    void InitNativeLight()
    {
        lightCache = GetComponent<Light>();
        lightData = new SqLightData();
        lightData.shadowMatrix = new Matrix4x4[4];
        lightData.color = lightCache.color.linear;
        lightData.type = (int)lightCache.type;
        lightData.intensity = lightCache.intensity;

        if (lightCache.type == LightType.Directional)
        {
            lightData.worldPos = transform.forward;
        }
        else
        {
            lightData.worldPos = transform.position;
        }

        nativeID = AddNativeLight(lightCache.GetInstanceID(), lightData);
    }

    void InitShadows()
    {
        if (lightCache.shadows == LightShadows.None || lightCache.type != LightType.Directional)
        {
            return;
        }

        if (cascadeSetting.Length > 4)
        {
            Debug.LogError("Max cascade is 4");
            return;
        }

        int size = shadowMapSize[(int)shadowSize];

        // create cascade shadows
        if (cascadeSetting.Length > 0)
        {
            shadowMaps = new RenderTexture[cascadeSetting.Length];
            cascadeLast = new float[cascadeSetting.Length];
            for (int i = 0; i < shadowMaps.Length; i++)
            {
                shadowMaps[i] = new RenderTexture(size, size, 32, RenderTextureFormat.Depth, RenderTextureReadWrite.Linear);
                shadowMaps[i].name = name + "_ShadowMap " + i;
                shadowMaps[i].Create();
                cascadeLast[i] = cascadeSetting[i];
            }
        }
        else
        {
            // otherwise create normal shadow maps
            shadowMaps = new RenderTexture[1];
            shadowMaps[0] = new RenderTexture(size, size, 32, RenderTextureFormat.Depth, RenderTextureReadWrite.Linear);
            shadowMaps[0].name = name + "_ShadowMap";
            shadowMaps[0].Create();
        }

        IntPtr[] shadowPtr = new IntPtr[shadowMaps.Length];
        for (int i = 0; i < shadowMaps.Length; i++)
        {
            shadowPtr[i] = shadowMaps[i].GetNativeDepthBufferPtr();
        }
        InitNativeShadows(nativeID, shadowMaps.Length, shadowPtr);

        // init shadow cam
        GameObject newObj = new GameObject();
        shadowCam = newObj.AddComponent<Camera>();
        shadowCam.transform.SetParent(lightCache.transform);
        shadowCam.transform.localPosition = Vector3.zero;
        shadowCam.transform.localRotation = Quaternion.identity;
        shadowCam.transform.localScale = Vector3.one;
        shadowCam.orthographic = true;
        shadowCam.targetDisplay = 3;
        shadowCam.aspect = 1f;
        shadowCam.cullingMask = 0;
        shadowCam.clearFlags = CameraClearFlags.Nothing;

        // change shadow cam's view port 
        shadowCam.targetTexture = shadowMaps[0];

        transform.hasChanged = true;    // force update once
    }

    void UpdateShadowMatrix()
    {
        if (lightCache.type != LightType.Directional)
        {
            return;
        }

        SetupCascade();

        // view port and scissor
        Rect viewRect = shadowCam.pixelRect;

        ViewPort vp;
        vp.TopLeftX = viewRect.xMin;
        vp.TopLeftY = viewRect.yMin;
        vp.Width = viewRect.width;
        vp.Height = viewRect.height;
        vp.MinDepth = 0f;
        vp.MaxDepth = 1f;

        RawRect rr;
        rr.left = 0;
        rr.top = 0;
        rr.right = (int)viewRect.width;
        rr.bottom = (int)viewRect.height;

        SetShadowViewPortScissorRect(nativeID, vp, rr);
    }

    void SetupCascade()
    {
        int numCascade = cascadeSetting.Length;
        numCascade = (numCascade == 0) ? 1 : numCascade;

        for (int i = 0; i < numCascade; i++)
        {
            float dist = mainCam.farClipPlane * ((cascadeSetting.Length == 0) ? 1f : cascadeSetting[i]);
            shadowCam.nearClipPlane = lightCache.shadowNearPlane;
            shadowCam.farClipPlane = dist;
            shadowCam.orthographicSize = dist;

            // position
            shadowCam.transform.position = mainCamTrans.position - lightCache.transform.forward * dist * 0.5f;
            lightData.shadowMatrix[i] = GL.GetGPUProjectionMatrix(shadowCam.projectionMatrix, true) * shadowCam.worldToCameraMatrix;

            SetShadowFrustum(nativeID, shadowCam.worldToCameraMatrix, GL.GetGPUProjectionMatrix(shadowCam.projectionMatrix, false), i);
        }

        lightData.numCascade = numCascade;

        UpdateNativeShadow(nativeID, lightData);
    }

    void UpdateNativeLight()
    {
        if (transform.hasChanged || LightChanged() || CascadeChanged())
        {
            if (lightCache.type == LightType.Directional)
            {
                lightData.worldPos = transform.forward;
            }
            else
            {
                lightData.worldPos = transform.position;
            }

            lightData.color = lightCache.color.linear;
            lightData.intensity = lightCache.intensity;

            UpdateNativeLight(nativeID, lightData);
            transform.hasChanged = false;
        }
    }

    bool LightChanged()
    {
        if (lightData.intensity != lightCache.intensity)
        {
            return true;
        }

        if (Vector4.SqrMagnitude(lightData.color - (Vector4)lightCache.color.linear) > 0.1)
        {
            return true;
        }

        return false;
    }

    bool CascadeChanged()
    {
        if (lightCache.type != LightType.Directional)
        {
            return false;
        }

        for (int i = 0; i < cascadeSetting.Length; i++)
        {
            if (cascadeSetting[i] != cascadeLast[i])
            {
                return true;
            }
        }

        return false;
    }
}
